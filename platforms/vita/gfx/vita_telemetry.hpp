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
  StatePipeline,   // GX -> PipelineDesc/vertex layout translation (subset of StateTranslate)
  StateKey,        // pipeline_key hashing during state translation
  StateVertex,     // matrices, lights, projection and uniform translation
  StateMemo,       // translation memo: snapshot hash, compare and copy (subset of StatePipeline)
  StateLayout,     // vertex decode layout build (subset of StatePipeline)
  Count,
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
  uint64_t pipelineTranslations = 0;
  uint64_t vertexTranslations = 0;
  uint64_t textureResolves = 0;
  uint64_t translationMemoHits = 0;
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
  void count_pipeline_translation() noexcept { ++frame_.counters.pipelineTranslations; }
  void count_vertex_translation() noexcept { ++frame_.counters.vertexTranslations; }
  void count_texture_resolve() noexcept { ++frame_.counters.textureResolves; }
  void count_translation_memo_hit() noexcept { ++frame_.counters.translationMemoHits; }
  void add_draw(uint32_t vertices, uint32_t indices, uint32_t triangles) noexcept;
  void vertex_dedup(uint32_t inputVertices,uint32_t uniqueVertices) noexcept;
  void gpu_geometry(bool hit,uint32_t vertices) noexcept;
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

// Per-frame profile of the GX FIFO command processor, collected only while
// set_fifo_profile_enabled(true) (the backend enables it with telemetry). Times are exclusive per command class: a display
// list call does not include the commands executed inside it.
enum class FifoCommandClass : uint8_t { Bp, Cp, Xf, Indexed, CallList, Draw, Aurora, Other, Count };
struct FifoProfile {
  uint64_t processUs = 0;     // outermost process() wall time
  uint64_t bytes = 0;
  uint64_t us[static_cast<size_t>(FifoCommandClass::Count)]{};
  uint32_t count[static_cast<size_t>(FifoCommandClass::Count)]{};
};
FifoProfile& fifo_profile_accumulator() noexcept;
extern bool g_fifoProfileEnabled;
inline void set_fifo_profile_enabled(bool enabled) noexcept { g_fifoProfileEnabled = enabled; }
// Returns the accumulated profile and clears the accumulator.
FifoProfile fifo_profile_take() noexcept;

} // namespace aurora::vita::gfx
