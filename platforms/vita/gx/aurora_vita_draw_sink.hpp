#pragma once

#include "aurora_gx_bridge.hpp"
#include "../gfx/vita_command_stream.hpp"
#include "../gfx/vita_streaming_arena.hpp"
#include "../gfx/vita_telemetry.hpp"
#include "../gfx/vita_memory_budget.hpp"
#include "../gfx/vita_static_geometry.hpp"
#include "../integration/vita_feature_coverage.hpp"
#include "../integration/vita_frame_trace.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include "../gfx/vita_hash_map.hpp"

namespace aurora::vita::gxbridge {

enum class SubmitWarning : uint8_t {
  None = 0,
  MissingTextureFallback = 1 << 0,
  DynamicCopyFallback = 1 << 1,
  LogicOpFallback = 1 << 2,
  OrigLodApproximation = 1 << 3,
  ZCompLocApproximation = 1 << 4,
};
inline SubmitWarning operator|(SubmitWarning a, SubmitWarning b) noexcept {
  return static_cast<SubmitWarning>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline SubmitWarning& operator|=(SubmitWarning& a, SubmitWarning b) noexcept { a = a | b; return a; }
inline bool has_warning(SubmitWarning set, SubmitWarning bit) noexcept {
  return (static_cast<uint8_t>(set) & static_cast<uint8_t>(bit)) != 0;
}

inline bool scissor_covers_full_target(const gfx::Scissor& scissor,
                                       bool defaultTarget,
                                       uint32_t targetWidth, uint32_t targetHeight,
                                       uint32_t renderWidth, uint32_t renderHeight) noexcept {
  const uint32_t width = defaultTarget ? renderWidth : targetWidth;
  const uint32_t height = defaultTarget ? renderHeight : targetHeight;
  return scissor.x <= 0 && scissor.y <= 0 &&
      int64_t(scissor.x) + scissor.width >= width &&
      int64_t(scissor.y) + scissor.height >= height;
}

struct SubmitResult {
  bool ok = false;
  gfx::PrepareDrawError drawError = gfx::PrepareDrawError::None;
  SubmitWarning warnings = SubmitWarning::None;
  uint8_t fallbackTextureMask = 0;
};

struct DrawSinkConfig {
  gfx::StreamingArenaConfig streaming{};
  size_t commandReserve = 2048;
  gfx::Telemetry* telemetry = nullptr;
  integration::FeatureCoverage* coverage = nullptr;
  integration::FrameTrace* trace = nullptr;
  // Human-readable geometry dumps intentionally walk prepared vertices and
  // write several stderr lines per sampled draw. Keep that independent from
  // structured telemetry/coverage so profiling does not perturb the hot path.
  bool verboseGeometryDiagnostics = false;
  bool strictUnsupported = false;
  size_t staticGeometryBudget = 0; // Zero keeps the established CPU vertex path.
  bool allowLitFixedVertexGpu = false;
  uint32_t diagnosticDrawLimit = 0;
};

class DrawSink {
public:
  DrawSink() = default;
  ~DrawSink();
  DrawSink(const DrawSink&) = delete;
  DrawSink& operator=(const DrawSink&) = delete;

  bool initialize(gfx::Renderer& renderer, const DrawSinkConfig& config = {}) noexcept;
  void shutdown() noexcept;
  void begin_frame(uint64_t frame) noexcept;
  void flush() noexcept;
  void reset_commands() noexcept { stream_.reset(); fixedVertexUniforms_.clear(); reset_pipeline_run_cache(); }
  void invalidate_texture_resolve_cache() noexcept {
#if defined(AURORA_VITA_UPSTREAM)
    resolvedTextureBindingsValid_ = false;
#endif
  }

#if defined(AURORA_VITA_UPSTREAM)
  SubmitResult submit(uint8_t primitive, uint8_t fmt, const uint8_t* rawVertices,
                      size_t rawBytes, uint32_t vertexCount,
                      const uint16_t* rawIndices = nullptr, uint32_t indexCount = 0,
                      const uint8_t* stableSource = nullptr) noexcept;
  // GXCopyTex integration. Call after the source EFB has been rendered and before
  // a texture object backed by dest is sampled. The backend owns the copied image;
  // native GXM currently uses a synchronized CPU conversion for color copies.
  bool copy_tex(const void* dest, bool clear) noexcept;
  void evict_copy_tex(const void* dest) noexcept;
  void clear_copy_textures() noexcept;
#endif

  gfx::CommandStream& stream() noexcept { return stream_; }
  const gfx::CommandStream& stream() const noexcept { return stream_; }
  uint64_t submitted_draws() const noexcept { return submittedDraws_; }
  bool strict_failed() const noexcept { return strictFailed_; }
  gfx::MemoryBudgetSnapshot memory_budget() const noexcept;

private:
  gfx::Handle white_texture() noexcept;
  void reset_pipeline_run_cache() noexcept {
#if defined(AURORA_VITA_UPSTREAM)
    queuedPipelineValid_ = false;
#endif
  }
  gfx::Renderer* renderer_ = nullptr;
  std::unique_ptr<gfx::StreamingArena> arena_{};
  gfx::CommandStream stream_{};
  gfx::PreparedDraw preparedScratch_{};
  std::unique_ptr<gfx::StaticGeometryCache> staticGeometry_{};
  std::deque<gfx::FixedVertexUniforms> fixedVertexUniforms_{};
  gfx::FlatHashMap<uint64_t,uint64_t> fixedPipelineKeys_{};
  gfx::VertexTransformState translatedVertexState_{};
  gfx::DrawUniforms translatedUniforms_{};
  uint32_t translatedUniformGeneration_ = 1;
  uint32_t resolvedTextureGeneration_ = 1;
  bool translatedVertexStateValid_ = false;
  bool translatedVertexStateLightweight_ = false;
  gfx::Handle whiteTexture_ = gfx::InvalidHandle;
#if defined(AURORA_VITA_UPSTREAM)
  struct PipelineTranslationEntry {
    uint64_t fingerprintLo = 0;
    uint64_t fingerprintHi = 0;
    uint64_t pipelineKey = 0;
    gfx::PipelineDesc pipeline{};
    gfx::VertexDecodeLayout layout{};
    gfx::PipelineDesc gpuPipeline{};
    uint8_t fmt = 0;
    uint8_t textureMask = 0;
    bool usesOrigLod = false;
    bool hasIndirect = false;
    bool lit = false;
    bool valid = false;
  };
  struct PipelineTranslationSet {
    std::array<PipelineTranslationEntry, 2> ways{};
    uint8_t mru = 0;
  };
  static constexpr size_t PipelineTranslationSetCount = 64;
  using PipelineTranslationCache = std::array<PipelineTranslationSet, PipelineTranslationSetCount>;

  struct TextureResolveStamp {
    uintptr_t data = 0;
    uint32_t mode0 = 0;
    uint32_t mode1 = 0;
    uint32_t image0 = 0;
    uint32_t image3 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t tlut = 0;
    uint32_t texObjId = 0;
    uint32_t texDataVersion = 0;
    uint8_t flags = 0;
    uintptr_t paletteData = 0;
    uint32_t paletteFormat = 0;
    uint32_t paletteEntries = 0;
    uint32_t paletteObjId = 0;
    uint32_t paletteDataVersion = 0;
    uint8_t paletteFlags = 0;
    bool gxCopyPresent = false;
    bool localCopyPresent = false;
    uint32_t localCopyRevision = 0;

    bool operator==(const TextureResolveStamp& rhs) const noexcept {
      return data == rhs.data && mode0 == rhs.mode0 && mode1 == rhs.mode1 &&
             image0 == rhs.image0 && image3 == rhs.image3 &&
             width == rhs.width && height == rhs.height && format == rhs.format &&
             tlut == rhs.tlut && texObjId == rhs.texObjId &&
             texDataVersion == rhs.texDataVersion && flags == rhs.flags &&
             paletteData == rhs.paletteData && paletteFormat == rhs.paletteFormat &&
             paletteEntries == rhs.paletteEntries && paletteObjId == rhs.paletteObjId &&
             paletteDataVersion == rhs.paletteDataVersion && paletteFlags == rhs.paletteFlags &&
             gxCopyPresent == rhs.gxCopyPresent && localCopyPresent == rhs.localCopyPresent &&
             localCopyRevision == rhs.localCopyRevision;
    }
    bool operator!=(const TextureResolveStamp& rhs) const noexcept { return !(*this == rhs); }
  };
  TextureResolveStamp texture_resolve_stamp(unsigned slot) const noexcept;
  std::unique_ptr<PipelineTranslationCache> pipelineTranslationCache_{};
  PipelineTranslationEntry* translatedEntry_ = nullptr;
  uint64_t translatedFingerprintLo_ = 0;
  uint64_t translatedFingerprintHi_ = 0;
  uint8_t translatedFmt_ = 0;
  std::array<gfx::TextureBinding,gfx::MaxTextures> resolvedTextureBindings_{};
  std::array<TextureResolveStamp,gfx::MaxTextures> resolvedTextureStamps_{};
  uint8_t resolvedTextureMask_ = 0;
  uint8_t resolvedVolatileTextureMask_ = 0;
  uint8_t resolvedFallbackTextureMask_ = 0;
  SubmitWarning resolvedTextureWarnings_ = SubmitWarning::None;
  bool resolvedTextureBindingsValid_ = false;
  uint64_t queuedTranslatedPipelineKey_ = 0;
  uint64_t queuedResolvedPipelineKey_ = 0;
  gfx::Primitive queuedPrimitive_ = gfx::Primitive::Triangles;
  bool queuedPositionIsClipSpace_ = false;
  bool queuedFragmentScissor_ = true;
  uint8_t queuedNativeTextureWrapMask_ = 0;
  bool queuedPipelineValid_ = false;
#endif
  uint64_t submittedDraws_ = 0;
#if defined(AURORA_VITA_NO_DIAGNOSTICS)
  static constexpr gfx::Telemetry* telemetry_ = nullptr;
  static constexpr integration::FeatureCoverage* coverage_ = nullptr;
  static constexpr integration::FrameTrace* trace_ = nullptr;
#else
  gfx::Telemetry* telemetry_ = nullptr;
  integration::FeatureCoverage* coverage_ = nullptr;
  integration::FrameTrace* trace_ = nullptr;
#endif
  bool verboseGeometryDiagnostics_ = false;
  bool strictUnsupported_ = false;
  bool strictFailed_ = false;
  bool allowLitFixedVertexGpu_ = false;
  uint32_t diagnosticDrawLimit_ = 0;
  uint32_t frameDrawIndex_ = 0;
  struct CopyTextureEntry {
    gfx::Handle handle=gfx::InvalidHandle;
    uint32_t width=0,height=0;
    uint32_t revision=0;
    bool logicalFlipX=false,logicalFlipY=false,forceOpaque=false;
    gfx::EfbCopyFormat sampleFormat=gfx::EfbCopyFormat::Passthrough;
  };
  gfx::FlatHashMap<uintptr_t,CopyTextureEntry> copyTextures_{};
  bool initialized_ = false;
};

} // namespace aurora::vita::gxbridge
