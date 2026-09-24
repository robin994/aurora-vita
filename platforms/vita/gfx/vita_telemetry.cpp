#include "vita_telemetry.hpp"
#include <algorithm>
#include <chrono>
#include <sstream>
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

namespace aurora::vita::gfx {

uint64_t telemetry_now_us() noexcept {
#if defined(__vita__)
  return sceKernelGetProcessTimeWide();
#else
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

const char* telemetry_phase_name(TelemetryPhase phase) noexcept {
  switch (phase) {
    case TelemetryPhase::VertexDecode: return "vertex_decode";
    case TelemetryPhase::VertexTransform: return "vertex_transform";
    case TelemetryPhase::TextureResolve: return "texture_resolve";
    case TelemetryPhase::PipelineResolve: return "pipeline_resolve";
    case TelemetryPhase::CommandBuild: return "command_build";
    case TelemetryPhase::Submit: return "submit";
    case TelemetryPhase::EfbCopy: return "efb_copy";
    case TelemetryPhase::Present: return "present";
    case TelemetryPhase::BufferUpload: return "buffer_upload";
    case TelemetryPhase::StreamWait: return "stream_wait";
    case TelemetryPhase::VertexPack: return "vertex_pack";
    case TelemetryPhase::GeometryCache: return "geometry_cache";
    case TelemetryPhase::DrawFrontend: return "draw_frontend";
    case TelemetryPhase::StateTranslate: return "state_translate";
    case TelemetryPhase::StatePipelineTranslate: return "state_pipeline_translate";
    case TelemetryPhase::StatePipelineBuild: return "state_pipeline_build";
    case TelemetryPhase::StatePipelineKey: return "state_pipeline_key";
    case TelemetryPhase::StatePipelineDerived: return "state_pipeline_derived";
    case TelemetryPhase::StateVertexLightweight: return "state_vertex_lightweight";
    case TelemetryPhase::StateVertexFull: return "state_vertex_full";
    case TelemetryPhase::StateVertexFallback: return "state_vertex_fallback";
    case TelemetryPhase::GeometryKey: return "geometry_key";
    case TelemetryPhase::GeometryValidate: return "geometry_validate";
    case TelemetryPhase::Count: break;
  }
  return "unknown";
}

void Telemetry::reset() noexcept { frame_ = {}; lifetime_ = {}; }
void Telemetry::begin_frame(uint64_t frame) noexcept { frame_ = {}; frame_.frame = frame; }
void Telemetry::end_frame(uint64_t totalUs) noexcept { frame_.totalUs = totalUs; ++frame_.counters.frames; ++lifetime_.frames; }
void Telemetry::add_time(TelemetryPhase phase, uint64_t us) noexcept {
  const auto idx = static_cast<size_t>(phase);
  if (idx < frame_.phaseUs.size()) frame_.phaseUs[idx] += us;
}
void Telemetry::add_draw(uint32_t vertices, uint32_t indices, uint32_t triangles) noexcept {
  ++frame_.counters.draws; ++lifetime_.draws;
  frame_.counters.vertices += vertices; lifetime_.vertices += vertices;
  frame_.counters.indices += indices; lifetime_.indices += indices;
  frame_.counters.triangles += triangles; lifetime_.triangles += triangles;
}
void Telemetry::vertex_dedup(uint32_t inputVertices,uint32_t uniqueVertices) noexcept {
  frame_.counters.vertexDedupInput+=inputVertices;lifetime_.vertexDedupInput+=inputVertices;
  frame_.counters.vertexDedupUnique+=uniqueVertices;lifetime_.vertexDedupUnique+=uniqueVertices;
}
void Telemetry::gpu_geometry(bool hit,uint32_t vertices) noexcept {
  if(hit){++frame_.counters.gpuGeometryHits;++lifetime_.gpuGeometryHits;}
  else {++frame_.counters.gpuGeometryMisses;++lifetime_.gpuGeometryMisses;}
  frame_.counters.gpuVertices+=vertices;lifetime_.gpuVertices+=vertices;
}
void Telemetry::fixed_vertex_candidate(uint32_t vertices) noexcept {
  frame_.counters.fixedCandidateVertices+=vertices;lifetime_.fixedCandidateVertices+=vertices;
}
void Telemetry::fixed_vertex_reject(FixedVertexReject reason,uint32_t vertices) noexcept {
  uint64_t* frame=nullptr;uint64_t* lifetime=nullptr;
  switch(reason) {
    case FixedVertexReject::NoCache: frame=&frame_.counters.fixedRejectNoCacheVertices;lifetime=&lifetime_.fixedRejectNoCacheVertices;break;
    case FixedVertexReject::LitDisabled: frame=&frame_.counters.fixedRejectLitVertices;lifetime=&lifetime_.fixedRejectLitVertices;break;
    case FixedVertexReject::SmallDraw: frame=&frame_.counters.fixedRejectSmallVertices;lifetime=&lifetime_.fixedRejectSmallVertices;break;
    case FixedVertexReject::IndexedDraw: frame=&frame_.counters.fixedRejectIndexedVertices;lifetime=&lifetime_.fixedRejectIndexedVertices;break;
    case FixedVertexReject::Primitive: frame=&frame_.counters.fixedRejectPrimitiveVertices;lifetime=&lifetime_.fixedRejectPrimitiveVertices;break;
    case FixedVertexReject::UnsupportedFeatures: frame=&frame_.counters.fixedRejectFeatureVertices;lifetime=&lifetime_.fixedRejectFeatureVertices;break;
  }
  if(frame&&lifetime){*frame+=vertices;*lifetime+=vertices;}
}
void Telemetry::cpu_fallback(uint32_t vertices) noexcept {
  frame_.counters.cpuFallbackVertices+=vertices;lifetime_.cpuFallbackVertices+=vertices;
  frame_.counters.cpuFallbackMaxVertices=std::max<uint64_t>(frame_.counters.cpuFallbackMaxVertices,vertices);
  lifetime_.cpuFallbackMaxVertices=std::max<uint64_t>(lifetime_.cpuFallbackMaxVertices,vertices);
}
void Telemetry::geometry_lookup(bool stable,uint64_t hashBytes) noexcept {
  if(stable){++frame_.counters.geometryStableLookups;++lifetime_.geometryStableLookups;}
  else {++frame_.counters.geometryContentLookups;++lifetime_.geometryContentLookups;}
  frame_.counters.geometryHashBytes+=hashBytes;lifetime_.geometryHashBytes+=hashBytes;
}
void Telemetry::geometry_validate(uint64_t rawCompareBytes,uint64_t snapshotCompareBytes,
                                  uint32_t revisionChecks) noexcept {
  frame_.counters.geometryRawCompareBytes+=rawCompareBytes;lifetime_.geometryRawCompareBytes+=rawCompareBytes;
  frame_.counters.geometrySnapshotCompareBytes+=snapshotCompareBytes;lifetime_.geometrySnapshotCompareBytes+=snapshotCompareBytes;
  frame_.counters.geometryRevisionChecks+=revisionChecks;lifetime_.geometryRevisionChecks+=revisionChecks;
}
void Telemetry::geometry_reject(GeometryRejectReason reason,uint32_t semantic) noexcept {
  ++frame_.counters.geometryRejects;++lifetime_.geometryRejects;
  uint64_t* frame=nullptr;uint64_t* lifetime=nullptr;
  switch(reason) {
    case GeometryRejectReason::AlreadyVolatile: frame=&frame_.counters.geometryRejectVolatile;lifetime=&lifetime_.geometryRejectVolatile;break;
    case GeometryRejectReason::LayoutMismatch: frame=&frame_.counters.geometryRejectLayout;lifetime=&lifetime_.geometryRejectLayout;break;
    case GeometryRejectReason::StableIdentity: frame=&frame_.counters.geometryRejectStableIdentity;lifetime=&lifetime_.geometryRejectStableIdentity;break;
    case GeometryRejectReason::StableRevision: frame=&frame_.counters.geometryRejectStableRevision;lifetime=&lifetime_.geometryRejectStableRevision;break;
    case GeometryRejectReason::RawContent: frame=&frame_.counters.geometryRejectRawContent;lifetime=&lifetime_.geometryRejectRawContent;break;
    case GeometryRejectReason::SnapshotRevision: frame=&frame_.counters.geometryRejectSnapshotRevision;lifetime=&lifetime_.geometryRejectSnapshotRevision;break;
    case GeometryRejectReason::SnapshotContent: frame=&frame_.counters.geometryRejectSnapshotContent;lifetime=&lifetime_.geometryRejectSnapshotContent;break;
    case GeometryRejectReason::EntryCapacity: frame=&frame_.counters.geometryRejectEntryCapacity;lifetime=&lifetime_.geometryRejectEntryCapacity;break;
    case GeometryRejectReason::ByteCapacity: frame=&frame_.counters.geometryRejectByteCapacity;lifetime=&lifetime_.geometryRejectByteCapacity;break;
    case GeometryRejectReason::BuildFailure: frame=&frame_.counters.geometryRejectBuildFailure;lifetime=&lifetime_.geometryRejectBuildFailure;break;
  }
  if(frame&&lifetime){++*frame;++*lifetime;}
  if((reason==GeometryRejectReason::SnapshotRevision||reason==GeometryRejectReason::SnapshotContent)&&semantic<64){
    const uint64_t bit=1ull<<semantic;
    frame_.counters.geometrySnapshotRejectSemanticMask|=bit;
    lifetime_.geometrySnapshotRejectSemanticMask|=bit;
  }
}
void Telemetry::geometry_cache_state(size_t entries,size_t bytes) noexcept {
  frame_.counters.geometryCacheEntries=entries;
  frame_.counters.geometryCacheBytes=bytes;
  lifetime_.geometryCacheEntries=std::max<uint64_t>(lifetime_.geometryCacheEntries,entries);
  lifetime_.geometryCacheBytes=std::max<uint64_t>(lifetime_.geometryCacheBytes,bytes);
}
void Telemetry::geometry_trim(uint32_t evictions,uint64_t bytes,bool waited) noexcept {
  frame_.counters.geometryEvictions+=evictions;lifetime_.geometryEvictions+=evictions;
  frame_.counters.geometryEvictedBytes+=bytes;lifetime_.geometryEvictedBytes+=bytes;
  if(waited){++frame_.counters.geometryTrimWaits;++lifetime_.geometryTrimWaits;}
}
void Telemetry::geometry_volatile_bypass() noexcept {
  ++frame_.counters.geometryVolatileBypasses;
  ++lifetime_.geometryVolatileBypasses;
}
void Telemetry::pipeline(bool hit) noexcept {
  if (hit) { ++frame_.counters.pipelineHits; ++lifetime_.pipelineHits; }
  else { ++frame_.counters.pipelineMisses; ++lifetime_.pipelineMisses; }
}
void Telemetry::frontend_pipeline_translate(bool hit) noexcept {
  auto& frame = hit ? frame_.counters.frontendPipelineTranslateHits : frame_.counters.frontendPipelineTranslateMisses;
  auto& lifetime = hit ? lifetime_.frontendPipelineTranslateHits : lifetime_.frontendPipelineTranslateMisses;
  ++frame;
  ++lifetime;
}
void Telemetry::frontend_pipeline_fingerprint(bool hit) noexcept {
  auto& frame = hit ? frame_.counters.frontendPipelineFingerprintHits : frame_.counters.frontendPipelineFingerprintMisses;
  auto& lifetime = hit ? lifetime_.frontendPipelineFingerprintHits : lifetime_.frontendPipelineFingerprintMisses;
  ++frame;
  ++lifetime;
}
void Telemetry::frontend_vertex_state_reuse() noexcept {
  ++frame_.counters.frontendVertexStateReuses;
  ++lifetime_.frontendVertexStateReuses;
}
void Telemetry::frontend_vertex_state_build(bool lightweight) noexcept {
  if (lightweight) {
    ++frame_.counters.frontendVertexStateLightBuilds;
    ++lifetime_.frontendVertexStateLightBuilds;
  } else {
    ++frame_.counters.frontendVertexStateFullBuilds;
    ++lifetime_.frontendVertexStateFullBuilds;
  }
}
void Telemetry::frontend_vertex_state_fallback() noexcept {
  ++frame_.counters.frontendVertexStateFallbackBuilds;
  ++lifetime_.frontendVertexStateFallbackBuilds;
}
void Telemetry::frontend_texture_binding(bool reused) noexcept {
  if (reused) {
    ++frame_.counters.frontendTextureBindingReuses;
    ++lifetime_.frontendTextureBindingReuses;
  } else {
    ++frame_.counters.frontendTextureBindingBuilds;
    ++lifetime_.frontendTextureBindingBuilds;
  }
}
void Telemetry::frontend_queued_pipeline(bool reused) noexcept {
  if (reused) {
    ++frame_.counters.frontendQueuedPipelineReuses;
    ++lifetime_.frontendQueuedPipelineReuses;
  } else {
    ++frame_.counters.frontendQueuedPipelineResolves;
    ++lifetime_.frontendQueuedPipelineResolves;
  }
}
void Telemetry::frontend_draw_path(bool gpuGeometry, bool streamed) noexcept {
  if (gpuGeometry) {
    ++frame_.counters.frontendGpuGeometryDraws;
    ++lifetime_.frontendGpuGeometryDraws;
  } else if (streamed) {
    ++frame_.counters.frontendStreamedDraws;
    ++lifetime_.frontendStreamedDraws;
  } else {
    ++frame_.counters.frontendPreparedDraws;
    ++lifetime_.frontendPreparedDraws;
  }
}
void Telemetry::texture(bool hit, bool uploaded, uint64_t uploadBytes) noexcept {
  if (hit) { ++frame_.counters.textureHits; ++lifetime_.textureHits; }
  else { ++frame_.counters.textureMisses; ++lifetime_.textureMisses; }
  if (uploaded) {
    ++frame_.counters.textureUploads; ++lifetime_.textureUploads;
    frame_.counters.textureUploadBytes += uploadBytes; lifetime_.textureUploadBytes += uploadBytes;
  }
}
void Telemetry::fallback_texture(uint32_t count) noexcept { frame_.counters.fallbackTextures += count; lifetime_.fallbackTextures += count; }
void Telemetry::efb_copy() noexcept { ++frame_.counters.efbCopies; ++lifetime_.efbCopies; }
void Telemetry::arena_overflow() noexcept { ++frame_.counters.arenaOverflows; ++lifetime_.arenaOverflows; }
void Telemetry::unsupported() noexcept { ++frame_.counters.unsupportedFeatures; ++lifetime_.unsupportedFeatures; }

std::string Telemetry::format_frame() const {
  std::ostringstream out;
  out << "[AURORA-VITA][FRAME] frame=" << frame_.frame
      << " total_us=" << frame_.totalUs
      << " draws=" << frame_.counters.draws
      << " vertices=" << frame_.counters.vertices
      << " indices=" << frame_.counters.indices
      << " triangles=" << frame_.counters.triangles
      << " dedup_in=" << frame_.counters.vertexDedupInput
      << " dedup_unique=" << frame_.counters.vertexDedupUnique
      << " gpu_geometry_hit=" << frame_.counters.gpuGeometryHits
      << " gpu_geometry_miss=" << frame_.counters.gpuGeometryMisses
      << " gpu_vertices=" << frame_.counters.gpuVertices
      << " fixed_candidate_v=" << frame_.counters.fixedCandidateVertices
      << " fixed_rej_nocache_v=" << frame_.counters.fixedRejectNoCacheVertices
      << " fixed_rej_lit_v=" << frame_.counters.fixedRejectLitVertices
      << " fixed_rej_small_v=" << frame_.counters.fixedRejectSmallVertices
      << " fixed_rej_indexed_v=" << frame_.counters.fixedRejectIndexedVertices
      << " fixed_rej_primitive_v=" << frame_.counters.fixedRejectPrimitiveVertices
      << " fixed_rej_feature_v=" << frame_.counters.fixedRejectFeatureVertices
      << " cpu_fallback_v=" << frame_.counters.cpuFallbackVertices
      << " cpu_fallback_max_v=" << frame_.counters.cpuFallbackMaxVertices
      << " geo_stable=" << frame_.counters.geometryStableLookups
      << " geo_content=" << frame_.counters.geometryContentLookups
      << " geo_hash_bytes=" << frame_.counters.geometryHashBytes
      << " geo_raw_cmp_bytes=" << frame_.counters.geometryRawCompareBytes
      << " geo_snap_cmp_bytes=" << frame_.counters.geometrySnapshotCompareBytes
      << " geo_rev_checks=" << frame_.counters.geometryRevisionChecks
      << " geo_reject=" << frame_.counters.geometryRejects
      << " geo_rej_volatile=" << frame_.counters.geometryRejectVolatile
      << " geo_rej_layout=" << frame_.counters.geometryRejectLayout
      << " geo_rej_stable_id=" << frame_.counters.geometryRejectStableIdentity
      << " geo_rej_stable_rev=" << frame_.counters.geometryRejectStableRevision
      << " geo_rej_raw=" << frame_.counters.geometryRejectRawContent
      << " geo_rej_snap_rev=" << frame_.counters.geometryRejectSnapshotRevision
      << " geo_rej_snap_content=" << frame_.counters.geometryRejectSnapshotContent
      << " geo_rej_entry_cap=" << frame_.counters.geometryRejectEntryCapacity
      << " geo_rej_byte_cap=" << frame_.counters.geometryRejectByteCapacity
      << " geo_rej_build=" << frame_.counters.geometryRejectBuildFailure
      << " geo_rej_sem_mask=" << frame_.counters.geometrySnapshotRejectSemanticMask
      << " geo_cache_entries=" << frame_.counters.geometryCacheEntries
      << " geo_cache_bytes=" << frame_.counters.geometryCacheBytes
      << " geo_evict=" << frame_.counters.geometryEvictions
      << " geo_evict_bytes=" << frame_.counters.geometryEvictedBytes
      << " geo_trim_wait=" << frame_.counters.geometryTrimWaits
      << " geo_volatile_bypass=" << frame_.counters.geometryVolatileBypasses
      << " pipeline_hit=" << frame_.counters.pipelineHits
      << " pipeline_miss=" << frame_.counters.pipelineMisses
      << " fe_pipe_xlat_hit=" << frame_.counters.frontendPipelineTranslateHits
      << " fe_pipe_xlat_miss=" << frame_.counters.frontendPipelineTranslateMisses
      << " fe_pipe_fp_hit=" << frame_.counters.frontendPipelineFingerprintHits
      << " fe_pipe_fp_miss=" << frame_.counters.frontendPipelineFingerprintMisses
      << " fe_vtx_reuse=" << frame_.counters.frontendVertexStateReuses
      << " fe_vtx_light=" << frame_.counters.frontendVertexStateLightBuilds
      << " fe_vtx_full=" << frame_.counters.frontendVertexStateFullBuilds
      << " fe_vtx_fallback=" << frame_.counters.frontendVertexStateFallbackBuilds
      << " fe_tex_reuse=" << frame_.counters.frontendTextureBindingReuses
      << " fe_tex_build=" << frame_.counters.frontendTextureBindingBuilds
      << " fe_qpipe_reuse=" << frame_.counters.frontendQueuedPipelineReuses
      << " fe_qpipe_resolve=" << frame_.counters.frontendQueuedPipelineResolves
      << " fe_gpu_geo_draw=" << frame_.counters.frontendGpuGeometryDraws
      << " fe_stream_draw=" << frame_.counters.frontendStreamedDraws
      << " fe_prepared_draw=" << frame_.counters.frontendPreparedDraws
      << " texture_hit=" << frame_.counters.textureHits
      << " texture_miss=" << frame_.counters.textureMisses
      << " texture_uploads=" << frame_.counters.textureUploads
      << " upload_bytes=" << frame_.counters.textureUploadBytes
      << " fallback_tex=" << frame_.counters.fallbackTextures
      << " efb_copy=" << frame_.counters.efbCopies
      << " arena_overflow=" << frame_.counters.arenaOverflows
      << " unsupported=" << frame_.counters.unsupportedFeatures;
  for (size_t i = 0; i < frame_.phaseUs.size(); ++i) {
    out << ' ' << telemetry_phase_name(static_cast<TelemetryPhase>(i)) << "_us=" << frame_.phaseUs[i];
  }
  return out.str();
}

std::string Telemetry::format_lifetime() const {
  std::ostringstream out;
  out << "[AURORA-VITA][LIFETIME] frames=" << lifetime_.frames
      << " draws=" << lifetime_.draws << " vertices=" << lifetime_.vertices
      << " indices=" << lifetime_.indices << " triangles=" << lifetime_.triangles
      << " dedup_in=" << lifetime_.vertexDedupInput << " dedup_unique=" << lifetime_.vertexDedupUnique
      << " pipeline_hit=" << lifetime_.pipelineHits << " pipeline_miss=" << lifetime_.pipelineMisses
      << " texture_hit=" << lifetime_.textureHits << " texture_miss=" << lifetime_.textureMisses
      << " texture_uploads=" << lifetime_.textureUploads << " upload_bytes=" << lifetime_.textureUploadBytes
      << " fallback_tex=" << lifetime_.fallbackTextures << " efb_copy=" << lifetime_.efbCopies
      << " arena_overflow=" << lifetime_.arenaOverflows << " unsupported=" << lifetime_.unsupportedFeatures;
  return out.str();
}

bool Telemetry::append_frame_log(const char* path) const noexcept {
  if (!path || !*path) return false;
  FILE* fp = std::fopen(path, "ab");
  if (!fp) return false;
  const auto line = format_frame();
  const bool ok = std::fwrite(line.data(), 1, line.size(), fp) == line.size() && std::fwrite("\n", 1, 1, fp) == 1;
  std::fclose(fp);
  return ok;
}

#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
ScopedTelemetryPhase::ScopedTelemetryPhase(Telemetry* telemetry, TelemetryPhase phase,
                                           TelemetryPhase aggregate) noexcept
    : telemetry_(telemetry), phase_(phase), aggregate_(aggregate),
      startUs_(telemetry ? telemetry_now_us() : 0) {}
ScopedTelemetryPhase::~ScopedTelemetryPhase() {
  if (!telemetry_) return;
  const uint64_t elapsed = telemetry_now_us() - startUs_;
  telemetry_->add_time(phase_, elapsed);
  if (aggregate_ != TelemetryPhase::Count && aggregate_ != phase_)
    telemetry_->add_time(aggregate_, elapsed);
}
#endif

} // namespace aurora::vita::gfx
