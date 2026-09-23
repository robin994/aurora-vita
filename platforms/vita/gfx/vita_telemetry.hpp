#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace aurora::vita::gfx {

enum class TelemetryPhase : uint8_t {
  VertexDecode,
  VertexTransform,
  TextureResolve,
  PipelineResolve,
  CommandBuild,
  Submit,
  EfbCopy,
  Present,
  BufferUpload,
  StreamWait,
  VertexPack,
  GeometryCache,
  DrawFrontend,
  StateTranslate,
  GeometryKey,
  GeometryValidate,
  Count,
};

enum class FixedVertexReject : uint8_t {
  NoCache,
  LitDisabled,
  SmallDraw,
  IndexedDraw,
  Primitive,
  UnsupportedFeatures,
};

enum class GeometryRejectReason : uint8_t {
  AlreadyVolatile,
  LayoutMismatch,
  StableIdentity,
  StableRevision,
  RawContent,
  SnapshotRevision,
  SnapshotContent,
  EntryCapacity,
  ByteCapacity,
  BuildFailure,
};

struct TelemetryCounters {
  uint64_t frames = 0;
  uint64_t draws = 0;
  uint64_t vertices = 0;
  uint64_t indices = 0;
  uint64_t triangles = 0;
  uint64_t vertexDedupInput = 0;
  uint64_t vertexDedupUnique = 0;
  uint64_t gpuGeometryHits=0,gpuGeometryMisses=0,gpuVertices=0;
  uint64_t fixedCandidateVertices=0;
  uint64_t fixedRejectNoCacheVertices=0,fixedRejectLitVertices=0,fixedRejectSmallVertices=0;
  uint64_t fixedRejectIndexedVertices=0,fixedRejectPrimitiveVertices=0,fixedRejectFeatureVertices=0;
  uint64_t cpuFallbackVertices=0,cpuFallbackMaxVertices=0;
  uint64_t geometryStableLookups=0,geometryContentLookups=0,geometryHashBytes=0;
  uint64_t geometryRawCompareBytes=0,geometrySnapshotCompareBytes=0,geometryRevisionChecks=0;
  uint64_t geometryRejects=0;
  uint64_t geometryRejectVolatile=0,geometryRejectLayout=0,geometryRejectStableIdentity=0;
  uint64_t geometryRejectStableRevision=0,geometryRejectRawContent=0;
  uint64_t geometryRejectSnapshotRevision=0,geometryRejectSnapshotContent=0;
  uint64_t geometryRejectEntryCapacity=0,geometryRejectByteCapacity=0,geometryRejectBuildFailure=0;
  uint64_t geometrySnapshotRejectSemanticMask=0;
  uint64_t geometryCacheEntries=0,geometryCacheBytes=0;
  uint64_t geometryEvictions=0,geometryEvictedBytes=0,geometryTrimWaits=0;
  uint64_t geometryVolatileBypasses=0;
  uint64_t pipelineHits = 0;
  uint64_t pipelineMisses = 0;
  uint64_t textureHits = 0;
  uint64_t textureMisses = 0;
  uint64_t textureUploads = 0;
  uint64_t textureUploadBytes = 0;
  uint64_t fallbackTextures = 0;
  uint64_t efbCopies = 0;
  uint64_t arenaOverflows = 0;
  uint64_t unsupportedFeatures = 0;
};

struct FrameTelemetry {
  uint64_t frame = 0;
  uint64_t totalUs = 0;
  std::array<uint64_t, static_cast<size_t>(TelemetryPhase::Count)> phaseUs{};
  TelemetryCounters counters{};
};

class Telemetry {
public:
  void reset() noexcept;
  void begin_frame(uint64_t frame) noexcept;
  void end_frame(uint64_t totalUs) noexcept;
  void add_time(TelemetryPhase phase, uint64_t us) noexcept;
  void add_draw(uint32_t vertices, uint32_t indices, uint32_t triangles) noexcept;
  void vertex_dedup(uint32_t inputVertices,uint32_t uniqueVertices) noexcept;
  void gpu_geometry(bool hit,uint32_t vertices) noexcept;
  void fixed_vertex_candidate(uint32_t vertices) noexcept;
  void fixed_vertex_reject(FixedVertexReject reason,uint32_t vertices) noexcept;
  void cpu_fallback(uint32_t vertices) noexcept;
  void geometry_lookup(bool stable,uint64_t hashBytes) noexcept;
  void geometry_validate(uint64_t rawCompareBytes,uint64_t snapshotCompareBytes,
                         uint32_t revisionChecks) noexcept;
  void geometry_reject(GeometryRejectReason reason,uint32_t semantic=UINT32_MAX) noexcept;
  void geometry_cache_state(size_t entries,size_t bytes) noexcept;
  void geometry_trim(uint32_t evictions,uint64_t bytes,bool waited) noexcept;
  void geometry_volatile_bypass() noexcept;
  void set_split_vertex_phases(bool enabled) noexcept { splitVertexPhases_ = enabled; }
  bool split_vertex_phases() const noexcept { return splitVertexPhases_; }
  void pipeline(bool hit) noexcept;
  void texture(bool hit, bool uploaded, uint64_t uploadBytes = 0) noexcept;
  void fallback_texture(uint32_t count = 1) noexcept;
  void efb_copy() noexcept;
  void arena_overflow() noexcept;
  void unsupported() noexcept;

  const FrameTelemetry& frame() const noexcept { return frame_; }
  const TelemetryCounters& lifetime() const noexcept { return lifetime_; }
  std::string format_frame() const;
  std::string format_lifetime() const;
  bool append_frame_log(const char* path) const noexcept;

private:
  FrameTelemetry frame_{};
  TelemetryCounters lifetime_{};
  bool splitVertexPhases_ = false;
};

class ScopedTelemetryPhase {
public:
  ScopedTelemetryPhase(Telemetry* telemetry, TelemetryPhase phase) noexcept;
  ~ScopedTelemetryPhase();
  ScopedTelemetryPhase(const ScopedTelemetryPhase&) = delete;
  ScopedTelemetryPhase& operator=(const ScopedTelemetryPhase&) = delete;
private:
  Telemetry* telemetry_ = nullptr;
  TelemetryPhase phase_ = TelemetryPhase::Submit;
  uint64_t startUs_ = 0;
};

uint64_t telemetry_now_us() noexcept;
const char* telemetry_phase_name(TelemetryPhase phase) noexcept;

} // namespace aurora::vita::gfx
