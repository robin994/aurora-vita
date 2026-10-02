#pragma once
#include "vita_gfx_types.hpp"
namespace aurora::vita::gfx {
inline void reset_native_frame_stats(FrameStats& stats,
    const std::array<uint64_t,FinishReasonCount>& finishCalls,
    const std::array<uint64_t,FinishReasonCount>& finishWaitUs) noexcept {
  stats={};
  stats.nativeFinishReasonCalls=finishCalls;stats.nativeFinishReasonWaitUs=finishWaitUs;
}
inline FrameStats compose_native_frame_stats(const FrameStats& facade,const FrameStats& native) noexcept {
  FrameStats result=native;
  result.pipelineHits=facade.pipelineHits;result.pipelineMisses=facade.pipelineMisses;
  result.textureHits=facade.textureHits;result.textureMisses=facade.textureMisses;result.textureUploads=facade.textureUploads;
  result.stateChanges=facade.stateChanges;
  return result;
}
} // namespace aurora::vita::gfx
