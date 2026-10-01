#pragma once
#include <cstdint>

namespace aurora::vita::gfx {
// Describes actual native bindings. Different PipelineDesc keys may share the
// same shader stages and fixed-function values within one GXM scene.
struct PipelineBindingState {
  const void* vertex=nullptr;
  const void* fragment=nullptr;
  uint32_t depthFunction=0,depthWrite=0,cull=0;
};
enum PipelineStateChange : uint8_t {
  BindVertex=1,BindFragment=2,BindDepthFunction=4,BindDepthWrite=8,BindCull=16,BindAll=31,
};
inline uint8_t pipeline_state_changes(const PipelineBindingState& previous,
                                      const PipelineBindingState& next,bool reset) noexcept {
  if(reset)return BindAll;
  return uint8_t((previous.vertex!=next.vertex?BindVertex:0)|
      (previous.fragment!=next.fragment?BindFragment:0)|
      (previous.depthFunction!=next.depthFunction?BindDepthFunction:0)|
      (previous.depthWrite!=next.depthWrite?BindDepthWrite:0)|
      (previous.cull!=next.cull?BindCull:0));
}
inline unsigned pipeline_setter_count(uint8_t changes) noexcept {
  return unsigned(bool(changes&BindVertex))+unsigned(bool(changes&BindFragment))+
      2u*unsigned(bool(changes&BindDepthFunction))+2u*unsigned(bool(changes&BindDepthWrite))+
      unsigned(bool(changes&BindCull));
}
} // namespace aurora::vita::gfx
